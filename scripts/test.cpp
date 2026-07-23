#include <iostream>
#include <vector>
#include <string>
using namespace std;

vector<int> find(int n, string a, string b)
{
    vector<int> result;
    int l = -1, r = -1;
    for (int j = 0; j < n; j++)
    {
        if (a[j] != b[j])
        {
            if (l == -1)
            {
                l = j;
            }
            r = j;
        }
    }
    for (int i = l + 1; i <= r; i++)
    {
        if (b[i] != b[i - 1])
        {
            return vector<int>{-1};
        }
    }
    if (l == -1)
    {
        return vector<int>{-1};
    }
    if (l == 0 && r == n - 1)
    {
        return vector<int>{1, n, b[r]};
    }
    else if (l == 0 && a[r + 1] != b[r])
    {
        return vector<int>{1, r + 1, b[r]};
    }
    else if (r == n - 1 && a[l - 1] != b[l])
    {
        return vector<int>{l + 1, n, b[l]};
    }
    else
    {
        if (a[l - 1] == b[l] || a[r + 1] == b[r])
        {
            return vector<int>{-1};
        }else
        {
            return vector<int>{l + 1, r + 1, b[l]};
        }
    }
    return result;
}

int main()
{
    int n;
    cin >> n;
    string a, b;
    cin >> a >> b;
    vector<int> result = find(n, a, b);
    if (result[0] == -1)
    {
        cout << -1 << endl;
    }
    else
    {
        cout << result[0] << " " << result[1] << " " << result[2] << endl;
    }
}
