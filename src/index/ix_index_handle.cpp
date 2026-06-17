/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int l = 0, r = get_size() - 1;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        int cmp = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp >= 0) {
            r = mid - 1;
        } else {
            l = mid + 1;
        }
    }
    return l;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int l = page_hdr->is_leaf ? 0 : 1;
    int r = get_size() - 1;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        int cmp = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp > 0) {
            r = mid - 1;
        } else {
            l = mid + 1;
        }
    }
    return l;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int idx = lower_bound(key);
    if (idx < get_size() && ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(idx);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int idx = upper_bound(key);
    return value_at(idx - 1);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= get_size());
    int num = get_size();
    for (int i = num - 1; i >= pos; --i) {
        set_key(i + n, get_key(i));
        set_rid(i + n, *get_rid(i));
    }
    for (int i = 0; i < n; ++i) {
        set_key(pos + i, key + i * file_hdr->col_tot_len_);
        set_rid(pos + i, rid[i]);
    }
    set_size(num + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int idx = lower_bound(key);
    int num = get_size();
    if (idx < num && ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return num;
    }
    insert_pair(idx, key, value);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    int num = get_size();
    for (int i = pos + 1; i < num; ++i) {
        set_key(i - 1, get_key(i));
        set_rid(i - 1, *get_rid(i));
    }
    set_size(num - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int idx = lower_bound(key);
    int num = get_size();
    if (idx < num && ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(idx);
    }
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    
    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    if (is_empty()) return false;
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::FIND, transaction);
    if (leaf == nullptr) return false;
    Rid *val = nullptr;
    bool found = leaf->leaf_lookup(key, &val);
    if (found && val != nullptr) {
        result->push_back(*val);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return found;
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查自动作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    if (is_empty()) {
        return std::make_pair(nullptr, false);
    }
    IxNodeHandle *curr = fetch_node(file_hdr_->root_page_);
    while (!curr->is_leaf_page()) {
        page_id_t next_page_no;
        if (find_first) {
            next_page_no = curr->value_at(0);
        } else {
            next_page_no = curr->internal_lookup(key);
        }
        buffer_pool_manager_->unpin_page(curr->get_page_id(), false);
        delete curr;
        curr = fetch_node(next_page_no);
    }
    return std::make_pair(curr, false);
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->parent = node->page_hdr->parent;
    new_node->page_hdr->next_free_page_no = IX_NO_PAGE;

    int total_size = node->get_size();
    int num_left = total_size / 2;
    int num_right = total_size - num_left;

    new_node->insert_pairs(0, node->get_key(num_left), node->get_rid(num_left), num_right);
    node->set_size(num_left);

    if (node->is_leaf_page()) {
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());
        node->set_next_leaf(new_node->get_page_no());
        if (new_node->get_next_leaf() != IX_NO_PAGE) {
            IxNodeHandle *next_node = fetch_node(new_node->get_next_leaf());
            next_node->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next_node->get_page_id(), true);
            delete next_node;
        }
        if (node->get_page_no() == file_hdr_->last_leaf_) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        for (int i = 0; i < num_right; ++i) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 本函数执行完毕后，new node and old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        IxNodeHandle *root = create_node();
        root->page_hdr->is_leaf = false;
        root->page_hdr->parent = IX_NO_PAGE;
        root->page_hdr->next_free_page_no = IX_NO_PAGE;
        
        root->insert_pair(0, old_node->get_key(0), Rid{old_node->get_page_no(), -1});
        root->insert_pair(1, key, Rid{new_node->get_page_no(), -1});
        
        page_id_t root_page_no = root->get_page_no();
        update_root_page_no(root_page_no);
        old_node->set_parent_page_no(root_page_no);
        new_node->set_parent_page_no(root_page_no);
        
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        delete root;
        return;
    }

    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    int rank = parent->find_child(old_node);
    parent->insert_pair(rank + 1, key, Rid{new_node->get_page_no(), -1});
    new_node->set_parent_page_no(parent->get_page_no());

    if (parent->get_size() >= parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
        delete new_parent;
    }
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    delete parent;
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::unique_lock<std::shared_mutex> lock(root_latch_);
    if (is_empty()) {
        IxNodeHandle *root = create_node();
        root->page_hdr->is_leaf = true;
        root->page_hdr->parent = IX_NO_PAGE;
        root->page_hdr->next_free_page_no = IX_NO_PAGE;
        root->set_next_leaf(IX_NO_PAGE);
        root->set_prev_leaf(IX_NO_PAGE);
        root->insert(key, value);
        
        page_id_t root_page_no = root->get_page_no();
        update_root_page_no(root_page_no);
        file_hdr_->first_leaf_ = root_page_no;
        file_hdr_->last_leaf_ = root_page_no;
        
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        delete root;
        return root_page_no;
    }

    auto [leaf, _root_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    int size = leaf->insert(key, value);
    page_id_t leaf_page_no = leaf->get_page_no();

    if (size >= leaf->get_max_size()) {
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
        delete new_leaf;
    }
    maintain_parent(leaf);
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return leaf_page_no;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::unique_lock<std::shared_mutex> lock(root_latch_);
    if (is_empty()) return false;
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    if (leaf == nullptr) return false;

    int old_size = leaf->get_size();
    int new_size = leaf->remove(key);
    bool deleted = (new_size < old_size);

    if (deleted) {
        if (leaf->get_size() > 0) {
            maintain_parent(leaf);
        }
        bool root_is_latched = false;
        coalesce_or_redistribute(leaf, transaction, &root_is_latched);
        if (leaf->page != nullptr) {
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
        }
    } else {
        if (leaf->page != nullptr) {
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        }
    }
    delete leaf;
    return deleted;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int rank = parent->find_child(node);

    int neighbor_rank = (rank > 0) ? (rank - 1) : (rank + 1);
    IxNodeHandle *neighbor = fetch_node(parent->value_at(neighbor_rank));

    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        redistribute(neighbor, node, parent, rank);
        if (parent->page != nullptr) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        }
        if (neighbor->page != nullptr) {
            buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        }
        delete parent;
        delete neighbor;
        return false;
    } else {
        bool parent_should_delete = coalesce(&neighbor, &node, &parent, rank, transaction, root_is_latched);
        if (parent->page != nullptr) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        }
        if (neighbor->page != nullptr) {
            buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        }
        delete parent;
        delete neighbor;
        return parent_should_delete;
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        page_id_t new_root_page_no = old_root_node->value_at(0);
        update_root_page_no(new_root_page_no);
        IxNodeHandle *new_root = fetch_node(new_root_page_no);
        new_root->set_parent_page_no(IX_NO_PAGE);
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        delete new_root;
        
        buffer_pool_manager_->unpin_page(old_root_node->get_page_id(), false);
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        release_node_handle(*old_root_node);
        old_root_node->page = nullptr;
        return true;
    }
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        update_root_page_no(IX_NO_PAGE);
        file_hdr_->first_leaf_ = IX_NO_PAGE;
        file_hdr_->last_leaf_ = IX_NO_PAGE;
        
        buffer_pool_manager_->unpin_page(old_root_node->get_page_id(), false);
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        release_node_handle(*old_root_node);
        old_root_node->page = nullptr;
        return true;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index > 0) {
        // neighbor_node is predecessor (left)
        int last_idx = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(last_idx), *neighbor_node->get_rid(last_idx));
        neighbor_node->erase_pair(last_idx);
        
        parent->set_key(index, node->get_key(0));
        maintain_child(node, 0);
    } else {
        // neighbor_node is successor (right)
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        
        parent->set_key(index + 1, neighbor_node->get_key(0));
        maintain_child(node, node->get_size() - 1);
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    if (index > 0) {
        // neighbor_node is left, node is right. Merge right into left.
        int num_left = (*neighbor_node)->get_size();
        int num_right = (*node)->get_size();

        (*neighbor_node)->insert_pairs(num_left, (*node)->get_key(0), (*node)->get_rid(0), num_right);
        
        for (int i = 0; i < num_right; ++i) {
            maintain_child(*neighbor_node, num_left + i);
        }

        if ((*node)->is_leaf_page()) {
            (*neighbor_node)->set_next_leaf((*node)->get_next_leaf());
            if ((*neighbor_node)->get_next_leaf() != IX_NO_PAGE) {
                IxNodeHandle *next_node = fetch_node((*neighbor_node)->get_next_leaf());
                next_node->set_prev_leaf((*neighbor_node)->get_page_no());
                buffer_pool_manager_->unpin_page(next_node->get_page_id(), true);
                delete next_node;
            }
            if ((*node)->get_page_no() == file_hdr_->last_leaf_) {
                file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
            }
        }

        buffer_pool_manager_->unpin_page((*node)->get_page_id(), false);
        buffer_pool_manager_->delete_page((*node)->get_page_id());
        release_node_handle(**node);
        (*node)->page = nullptr;

        (*parent)->erase_pair(index);
    } else {
        // node is left, neighbor_node is right. Merge right into left.
        int num_left = (*node)->get_size();
        int num_right = (*neighbor_node)->get_size();

        (*node)->insert_pairs(num_left, (*neighbor_node)->get_key(0), (*neighbor_node)->get_rid(0), num_right);
        
        for (int i = 0; i < num_right; ++i) {
            maintain_child(*node, num_left + i);
        }

        if ((*neighbor_node)->is_leaf_page()) {
            (*node)->set_next_leaf((*neighbor_node)->get_next_leaf());
            if ((*node)->get_next_leaf() != IX_NO_PAGE) {
                IxNodeHandle *next_node = fetch_node((*node)->get_next_leaf());
                next_node->set_prev_leaf((*node)->get_page_no());
                buffer_pool_manager_->unpin_page(next_node->get_page_id(), true);
                delete next_node;
            }
            if ((*neighbor_node)->get_page_no() == file_hdr_->last_leaf_) {
                file_hdr_->last_leaf_ = (*node)->get_page_no();
            }
        }

        buffer_pool_manager_->unpin_page((*neighbor_node)->get_page_id(), false);
        buffer_pool_manager_->delete_page((*neighbor_node)->get_page_id());
        release_node_handle(**neighbor_node);
        (*neighbor_node)->page = nullptr;

        (*parent)->set_key(0, (*node)->get_key(0)); // Update parent key at index 0
        (*parent)->erase_pair(1); // neighbor_node is index 1
        
        maintain_parent(*parent); // Propagate first key change of parent up
    }

    bool parent_should_delete = false;
    if ((*parent)->is_root_page()) {
        parent_should_delete = adjust_root(*parent);
    } else if ((*parent)->get_size() < (*parent)->get_min_size()) {
        parent_should_delete = coalesce_or_redistribute(*parent, transaction, root_is_latched);
    }
    return parent_should_delete;
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    Rid rid = *node->get_rid(iid.slot_no);
    delete node;
    return rid;
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key 全量索引列拼接后的复合 key（长度为 file_hdr_->col_tot_len_）
 * @return Iid 指向第一个 >= key 的叶子节点中的位置；若不存在则返回 leaf_end()
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    if (is_empty()) return leaf_end();
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int idx = leaf->lower_bound(key);
    Iid iid;
    if (idx >= leaf->get_size()) {
        // 当前叶子所有 key 都小于目标：跳到下一个叶子的首位
        page_id_t next = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        if (next == IX_NO_PAGE || next == IX_LEAF_HEADER_PAGE) {
            return leaf_end();
        }
        iid = {.page_no = next, .slot_no = 0};
        return iid;
    }
    iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key 全量复合 key
 * @return Iid 指向第一个 > key 的位置。
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    if (is_empty()) return leaf_end();
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int idx = leaf->upper_bound(key);
    Iid iid;
    if (idx >= leaf->get_size()) {
        page_id_t next = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        if (next == IX_NO_PAGE || next == IX_LEAF_HEADER_PAGE) {
            return leaf_end();
        }
        iid = {.page_no = next, .slot_no = 0};
        return iid;
    }
    iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    delete node;
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan the first
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), false);
            delete parent;
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        
        if (curr != node) {
            buffer_pool_manager_->unpin_page(curr->get_page_id(), true);
            delete curr;
        }
        curr = parent;
    }
    if (curr != node) {
        buffer_pool_manager_->unpin_page(curr->get_page_id(), true);
        delete curr;
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    delete prev;

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    delete next;
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
    }
}