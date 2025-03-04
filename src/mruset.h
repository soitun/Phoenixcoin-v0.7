// Copyright (c) 2012 The Bitcoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file LICENCE or http://opensource.org/license/mit

#ifndef MRUSET_H
#define MRUSET_H

#include <deque>
#include <utility>
#include <set>

/** STL-like set container that only keeps the most recent N elements. */
template <typename T> class mruset
{
public:
    typedef T key_type;
    typedef T value_type;
    typedef typename std::set<T>::iterator iterator;
    typedef typename std::set<T>::const_iterator const_iterator;
    typedef typename std::set<T>::size_type size_type;

protected:
    std::set<T> set;
    std::deque<T> queue;
    size_type nMaxSize;

public:
    mruset(size_type nMaxSizeIn = 0) { nMaxSize = nMaxSizeIn; }
    iterator begin() const { return set.begin(); }
    iterator end() const { return set.end(); }
    size_type size() const { return set.size(); }
    bool empty() const { return set.empty(); }
    iterator find(const key_type& k) const { return set.find(k); }
    size_type count(const key_type& k) const { return set.count(k); }
    bool inline friend operator==(const mruset<T>& a, const mruset<T>& b) { return a.set == b.set; }
    bool inline friend operator==(const mruset<T>& a, const std::set<T>& b) { return a.set == b; }
    bool inline friend operator<(const mruset<T>& a, const mruset<T>& b) { return a.set < b.set; }
    std::pair<iterator, bool> insert(const key_type& x)
    {
        std::pair<iterator, bool> ret = set.insert(x);
        if (ret.second)
        {
            if (nMaxSize && queue.size() == nMaxSize)
            {
                set.erase(queue.front());
                queue.pop_front();
            }
            queue.push_back(x);
        }
        return ret;
    }
    size_type max_size() const { return nMaxSize; }
    size_type max_size(size_type s)
    {
        if (s)
            while (queue.size() > s)
            {
                set.erase(queue.front());
                queue.pop_front();
            }
        nMaxSize = s;
        return nMaxSize;
    }
};

#endif /* MRUSET_H */
